#include "drake/examples/kuka_iiwa_arm/dev/monolithic_pick_and_place/action_primitives/action_primitive_base.h"

#include <utility>
#include <vector>

#include "drake/examples/kuka_iiwa_arm/dev/monolithic_pick_and_place/action_primitives/action_primitives_common.h"
#include "drake/examples/kuka_iiwa_arm/iiwa_common.h"
#include "drake/systems/framework/leaf_system.h"

using robotlocomotion::robot_plan_t;

namespace drake {
using systems::LeafSystem;

namespace examples {
namespace kuka_iiwa_arm {
namespace monolithic_pick_and_place {

ActionPrimitive::ActionPrimitive(double desired_update_interval,
                                 unsigned int action_primitive_state_index)
    : action_primitive_state_index_(action_primitive_state_index),
      status_output_port_(this->DeclareAbstractOutputPort().get_index()),
      update_interval_(desired_update_interval) {
  this->DeclarePeriodicUnrestrictedUpdate(update_interval_, 0);
}

std::unique_ptr<systems::AbstractValues>
ActionPrimitive::AllocateAbstractState() const {
  std::vector<std::unique_ptr<systems::AbstractValue>> abstract_vals =
      AllocateExtendedAbstractState();
  const ActionPrimitiveState default_state = ActionPrimitiveState::WAITING;
  abstract_vals.push_back(std::unique_ptr<systems::AbstractValue>(
      std::make_unique<systems::Value<ActionPrimitiveState>>(default_state)));
  return std::make_unique<systems::AbstractValues>(std::move(abstract_vals));
}

void ActionPrimitive::SetDefaultState(const systems::Context<double>& context,
                                      systems::State<double>* state) const {
  ActionPrimitiveState& action_state =
      state->get_mutable_abstract_state<ActionPrimitiveState>(
          action_primitive_state_index_);
  action_state = ActionPrimitiveState::WAITING;
  SetExtendedDefaultState(context, state);
}

void ActionPrimitive::DoCalcOutput(
    const systems::Context<double>& context,
    systems::SystemOutput<double>* output) const {
  // Perform basic calc output logic, i.e set primitive state output.
  ActionPrimitiveState& primitive_state_output =
      output->GetMutableData(status_output_port_)
          ->GetMutableValue<ActionPrimitiveState>();
  primitive_state_output = context.get_abstract_state<ActionPrimitiveState>(
      action_primitive_state_index_);

  // Call DoExtendedCalcOutput.
  DoExtendedCalcOutput(context, output);
}

std::unique_ptr<systems::AbstractValue> ActionPrimitive::AllocateOutputAbstract(
    const systems::OutputPortDescriptor<double>& descriptor) const {
  if (descriptor.get_index() == status_output_port_) {
    return systems::AbstractValue::Make<ActionPrimitiveState>(
        ActionPrimitiveState::WAITING);
  } else {
    return ExtendedAllocateOutputAbstract(descriptor);
  }
}

}  // namespace monolithic_pick_and_place
}  // namespace kuka_iiwa_arm
}  // namespace examples
}  // namespace drake
