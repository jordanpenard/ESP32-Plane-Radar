#include "services/unit_policy.h"

#include "ui/radar_range.h"

namespace services::units {

bool useImperialDistance() { return ui::radar::useMiles(); }

}  // namespace services::units
