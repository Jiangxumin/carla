// Copyright (c) 2020 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "StopSignComponent.h"
#include "TrafficLightState.h"
#include <queue>

#include <compiler/disable-ue4-macros.h>
#include <carla/road/element/RoadInfoSpeed.h>
#include <compiler/enable-ue4-macros.h>

void UStopSignComponent::InitializeSign(const carla::road::Map &Map)
{
  carla::log_warning("StopSign");

  const double epsilon = 0.00001;

  auto References = GetAllReferencesToThisSignal(Map);

  for (auto& Reference : References)
  {
    auto RoadId = Reference.first;
    const auto* SignalReference = Reference.second;
    TSet<carla::road::RoadId> SignalPredecessors;
    carla::log_warning("Stop:", RoadId);
    // Stop box
    for(auto &validity : SignalReference->GetValidities())
    {
      for(auto lane : carla::geom::Math::GenerateRange(validity._from_lane, validity._to_lane))
      {
        if(lane == 0)
          continue;

        auto signal_waypoint = Map.GetWaypoint(
            RoadId, lane, SignalReference->GetS()).get();

        // Get 90% of the half size of the width of the lane
        float BoxSize = static_cast<float>(
            0.9*Map.GetLaneWidth(signal_waypoint)/2.0);
        // Get min and max
        double LaneLength = Map.GetLane(signal_waypoint).GetLength();
        double LaneDistance = Map.GetLane(signal_waypoint).GetDistance();
        if(lane < 0)
        {
          signal_waypoint.s = FMath::Clamp(signal_waypoint.s - BoxSize,
              LaneDistance + epsilon, LaneDistance + LaneLength - epsilon);
        }
        else
        {
          signal_waypoint.s = FMath::Clamp(signal_waypoint.s + BoxSize,
              LaneDistance + epsilon, LaneDistance + LaneLength - epsilon);
        }
        float UnrealBoxSize = 100*BoxSize;
        GenerateStopBox(Map.ComputeTransform(signal_waypoint), UnrealBoxSize);

        auto Predecessors = Map.GetPredecessors(signal_waypoint);
        for(auto &Prev : Predecessors)
        {
          if(!SignalPredecessors.Contains(Prev.road_id))
          {
            SignalPredecessors.Add(Prev.road_id);
          }
        }
      }
    }

    //Check boxes
    if(Map.IsJunction(RoadId))
    {
      auto JuncId = Map.GetJunctionId(RoadId);
      const auto * Junction = Map.GetJunction(JuncId);
      if(Junction->RoadHasConflicts(RoadId))
      {
        const auto &ConflictingRoads = Junction->GetConflictsOfRoad(RoadId);
        for(const auto &Conflict : ConflictingRoads)
        {
          auto Waypoints = Map.GenerateWaypointsInRoad(Conflict);
          for(auto& Waypoint : Waypoints)
          {
            // Skip roads that share the same previous road
            bool bHasSamePredecessor = false;
            auto Predecessors = Map.GetPredecessors(Waypoint);
            for(auto &Prev : Predecessors)
            {
              if(SignalPredecessors.Contains(Prev.road_id))
              {
                bHasSamePredecessor = true;
              }
            }
            if(bHasSamePredecessor)
            {
              continue;
            }

            // Cover the road within the junction
            auto CurrentWaypoint = Waypoint;
            auto NextWaypoint = CurrentWaypoint;
            float BoxSize = static_cast<float>(
                0.9*Map.GetLaneWidth(NextWaypoint)/2.0);
            float UEBoxSize = 100*BoxSize;
            GenerateCheckBox(Map.ComputeTransform(NextWaypoint), UEBoxSize);
            while (true)
            {
              auto Next = Map.GetNext(NextWaypoint, 2*BoxSize);
              if (Next.size() != 1)
              {
                break;
              }
              NextWaypoint = Next.front();
              if(NextWaypoint.road_id != Waypoint.road_id)
              {
                break;
              }

              GenerateCheckBox(Map.ComputeTransform(NextWaypoint), UEBoxSize);
            }
            // Cover the road before the junction
            double AnticipationTime = 0.1;
            auto Previous = Map.GetPrevious(Waypoint, 2*BoxSize);
            std::queue<std::pair<float, carla::road::element::Waypoint>>
                WaypointQueue;
            for (auto & Prev : Previous)
            {
              WaypointQueue.push({AnticipationTime, Prev});
            }
            while (!WaypointQueue.empty())
            {
              auto CurrentWaypoint = WaypointQueue.front();
              WaypointQueue.pop();
              GenerateCheckBox(Map.ComputeTransform(CurrentWaypoint.second), UEBoxSize);

              float Speed = Map.GetLane(CurrentWaypoint.second).GetRoad()->GetInfo<carla::road::element::RoadInfoSpeed>(CurrentWaypoint.second.s)->GetSpeed();
              carla::log_warning("Speed:", Speed);
              float RemainingTime = CurrentWaypoint.first - BoxSize/Speed;
              if(RemainingTime > 0)
              {
                Previous = Map.GetPrevious(CurrentWaypoint.second, 2*BoxSize);
                for (auto & Prev : Previous)
                {
                  WaypointQueue.push({RemainingTime, Prev});
                }
              }
            }
          }
        }
      }
    }
  }
}

void UStopSignComponent::GenerateStopBox(const FTransform BoxTransform,
    float BoxSize)
{
  UBoxComponent* BoxComponent = GenerateTriggerBox(BoxTransform, BoxSize);
  BoxComponent->OnComponentBeginOverlap.AddDynamic(this, &UStopSignComponent::OnOverlapBeginStopEffectBox);
  BoxComponent->OnComponentEndOverlap.AddDynamic(this, &UStopSignComponent::OnOverlapEndStopEffectBox);

  DrawDebugBox(GetWorld(),
      BoxTransform.GetLocation(),
      BoxComponent->GetScaledBoxExtent(),
      BoxTransform.GetRotation(),
      FColor(0, 200, 0),
      true);
}

void UStopSignComponent::GenerateCheckBox(const FTransform BoxTransform,
    float BoxSize)
{
  UBoxComponent* BoxComponent = GenerateTriggerBox(BoxTransform, BoxSize);
  BoxComponent->OnComponentBeginOverlap.AddDynamic(this, &UStopSignComponent::OnOverlapBeginStopCheckBox);
  BoxComponent->OnComponentEndOverlap.AddDynamic(this, &UStopSignComponent::OnOverlapEndStopCheckBox);
  DrawDebugBox(GetWorld(),
      BoxTransform.GetLocation(),
      BoxComponent->GetScaledBoxExtent(),
      BoxTransform.GetRotation(),
      FColor(0, 0, 200),
      true);
}

void UStopSignComponent::GiveWayIfPossible()
{
  if (VehiclesToCheck.Num() == 0)
  {
    for (auto Vehicle : VehiclesInStop)
    {
      AWheeledVehicleAIController* VehicleController =
        Cast<AWheeledVehicleAIController>(Vehicle->GetController());
      VehicleController->SetTrafficLightState(ETrafficLightState::Green);
    }
  }
  else
  {
    if(VehiclesInStop.Num())
    {
      for (auto Vehicle : VehiclesInStop)
      {
        AWheeledVehicleAIController* VehicleController =
          Cast<AWheeledVehicleAIController>(Vehicle->GetController());
        VehicleController->SetTrafficLightState(ETrafficLightState::Red);
      }

      FTimerHandle TimerHandler;
      // 1 second delay
      GetWorld()->GetTimerManager().SetTimer(TimerHandler, this, &UStopSignComponent::GiveWayIfPossible, 1.0f);
    }
  }
}

void UStopSignComponent::OnOverlapBeginStopEffectBox(UPrimitiveComponent *OverlappedComp,
    AActor *OtherActor,
    UPrimitiveComponent *OtherComp,
    int32 OtherBodyIndex,
    bool bFromSweep,
    const FHitResult &SweepResult)
{
  ACarlaWheeledVehicle * Vehicle = Cast<ACarlaWheeledVehicle>(OtherActor);
  if (Vehicle)
  {
    AWheeledVehicleAIController* VehicleController =
        Cast<AWheeledVehicleAIController>(Vehicle->GetController());
    if (VehicleController)
    {
      VehicleController->SetTrafficLightState(ETrafficLightState::Red);
      VehiclesInStop.Add(Vehicle);

      FTimerHandle TimerHandler;
      // 2 second delay for stop
      GetWorld()->GetTimerManager().SetTimer(TimerHandler, this, &UStopSignComponent::GiveWayIfPossible, 2.0f);
    }
  }
  RemoveSameVehicleInBothLists();
}

void UStopSignComponent::OnOverlapEndStopEffectBox(UPrimitiveComponent *OverlappedComp,
    AActor *OtherActor,
    UPrimitiveComponent *OtherComp,
    int32 OtherBodyIndex)
{
  ACarlaWheeledVehicle * Vehicle = Cast<ACarlaWheeledVehicle>(OtherActor);
  if (Vehicle)
  {
    VehiclesInStop.Remove(Vehicle);
  }
}

void UStopSignComponent::OnOverlapBeginStopCheckBox(UPrimitiveComponent *OverlappedComp,
    AActor *OtherActor,
    UPrimitiveComponent *OtherComp,
    int32 OtherBodyIndex,
    bool bFromSweep,
    const FHitResult &SweepResult)
{
  ACarlaWheeledVehicle * Vehicle = Cast<ACarlaWheeledVehicle>(OtherActor);
  if (Vehicle)
  {
    if(!VehiclesInStop.Contains(Vehicle))
    {
      if (!VehiclesToCheck.Contains(Vehicle))
      {
        VehiclesToCheck.Add(Vehicle, 0);
      }
      VehiclesToCheck[Vehicle]++;
    }
    GiveWayIfPossible();
  }
}

void UStopSignComponent::OnOverlapEndStopCheckBox(UPrimitiveComponent *OverlappedComp,
    AActor *OtherActor,
    UPrimitiveComponent *OtherComp,
    int32 OtherBodyIndex)
{
  ACarlaWheeledVehicle * Vehicle = Cast<ACarlaWheeledVehicle>(OtherActor);
  if (Vehicle)
  {
    if(VehiclesToCheck.Contains(Vehicle))
    {
      VehiclesToCheck[Vehicle]--;
      if(VehiclesToCheck[Vehicle] <= 0)
      {
        VehiclesToCheck.Remove(Vehicle);
      }
    }
    GiveWayIfPossible();
  }
}
void UStopSignComponent::RemoveSameVehicleInBothLists()
{
  for(auto* Vehicle : VehiclesInStop)
  {
    if(VehiclesToCheck.Contains(Vehicle))
    {
      VehiclesToCheck.Remove(Vehicle);
    }
  }
}
