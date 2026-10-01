    .text
    .org 0xfa38
    tjl ramp_temp_display
    .org 0xfa3c
    tjl ramp_secondary_or_skip
    .org 0xfa50
    tjl ramp_countdown_or_skip
    .org 0xfa54
    tjl ramp_badge_or_skip
    .org 0xfa58
    tjl ramp_status_icon_or_skip
