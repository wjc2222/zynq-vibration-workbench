# Planning scaffold: read-only Vivado project/design inventory.
# Fill the expectations from this project's contract before sourcing.
# This does NOT create a project/IP, program hardware, or prove timing/CDC.
# It is not a verified portable overlay or a PYNQ implementation.
set expected_part ""
set expected_tool_version ""
# Tcl dict: exact clock name -> expected period in ns. At least one is required.
set required_clocks [dict create]
# Tcl dict: exact top-level port name -> IN, OUT, or INOUT.
set required_ports [dict create]
# Optional cell patterns; each must resolve to at least one hierarchical cell.
set required_cell_patterns [list]
set period_tolerance_ns 0.001

proc planning_fail {message} {
    return -code error "INTERFACE_INVENTORY_FAILED: $message"
}

if {$expected_part eq ""} {
    planning_fail "expected_part is unknown; fill the target contract first"
}
if {[dict size $required_clocks] == 0} {
    planning_fail "required_clocks is empty; fill actual clock expectations"
}
if {![string is double -strict $period_tolerance_ns] ||
    $period_tolerance_ns < 0} {
    planning_fail "period_tolerance_ns must be a nonnegative number"
}
if {[catch {current_project} project] || $project eq ""} {
    planning_fail "open the intended Vivado project first"
}
if {[catch {current_design} design] || $design eq ""} {
    planning_fail "open its synthesized or implemented design first"
}

set actual_part [get_property PART $project]
if {![string equal -nocase $actual_part $expected_part]} {
    planning_fail "project part mismatch: actual=$actual_part expected=$expected_part"
}
set actual_tool_version [version -short]
if {$expected_tool_version ne "" &&
    $actual_tool_version ne $expected_tool_version} {
    planning_fail "tool mismatch: actual=$actual_tool_version expected=$expected_tool_version"
}

set clock_inventory [dict create]
dict for {clock_name expected_period} $required_clocks {
    if {![string is double -strict $expected_period] || $expected_period <= 0} {
        planning_fail "clock $clock_name requires a positive numeric period"
    }
    set clock_object [get_clocks -quiet $clock_name]
    if {[llength $clock_object] != 1 ||
        [get_property NAME $clock_object] ne $clock_name} {
        planning_fail "clock must resolve exactly once: $clock_name"
    }
    set actual_period [get_property PERIOD $clock_object]
    if {![string is double -strict $actual_period] ||
        abs($actual_period - $expected_period) > $period_tolerance_ns} {
        planning_fail "clock period mismatch: $clock_name actual=$actual_period expected=$expected_period"
    }
    dict set clock_inventory $clock_name $actual_period
}

set port_inventory [dict create]
dict for {port_name expected_direction} $required_ports {
    if {$expected_direction ni {IN OUT INOUT}} {
        planning_fail "unsupported port direction: $expected_direction"
    }
    set port_object [get_ports -quiet $port_name]
    if {[llength $port_object] != 1 ||
        [get_property NAME $port_object] ne $port_name} {
        planning_fail "port must resolve exactly once: $port_name"
    }
    set actual_direction [get_property DIRECTION $port_object]
    if {$actual_direction ne $expected_direction} {
        planning_fail "port direction mismatch: $port_name"
    }
    dict set port_inventory $port_name $actual_direction
}

set cell_inventory [dict create]
foreach pattern $required_cell_patterns {
    set matches [get_cells -hierarchical -quiet $pattern]
    if {[llength $matches] == 0} {
        planning_fail "no hierarchical cell matched: $pattern"
    }
    dict set cell_inventory $pattern [llength $matches]
}
puts [dict create status inventory_matched part $actual_part \
    tool $actual_tool_version design $design clocks $clock_inventory \
    ports $port_inventory cells $cell_inventory]
puts "LIMIT: query inventory only; inspect actual reports, constraints, CDC, DRC, AXI behavior and hardware separately."
