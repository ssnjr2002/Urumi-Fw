
| Bit | Name (RDSEL=%00) | Name (%01) | Name (%10) | Read Response Function | Comment |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **19** | MSTEP9 | SG9 | SG9 | Microstep counter for coil A | Microstep position in sine table for coil A in STEP/DIR mode. MSTEP9 is the Polarity bit:<br>0: Current flows from OA1 pins to OA2 pins.<br>1: Current flows from OA2 pins to OA1 pins. |
| **18** | MSTEP8 | SG8 | SG8 | Microstep counter for coil A<br>*or*<br>stallGuard2 value SG9:0 | stallGuard2 value SG9:0. |
| **17** | MSTEP7 | SG7 | SG7 | Microstep counter for coil A<br>*or*<br>stallGuard2 value SG9:0 | stallGuard2 value SG9:0. |
| **16** | MSTEP6 | SG6 | SG6 | Microstep counter for coil A<br>*or*<br>stallGuard2 value SG9:0 | stallGuard2 value SG9:0. |
| **15** | MSTEP5 | SG5 | SG5 | Microstep counter for coil A<br>*or*<br>stallGuard2 value SG9:5 and coolStep value SE4:0 | stallGuard2 value SG9:5 and the actual coolStep scaling value SE4:0. |
| **14** | MSTEP4 | SG4 | SE4 | Microstep counter for coil A<br>*or*<br>stallGuard2 value SG9:5 and coolStep value SE4:0 | stallGuard2 value SG9:5 and the actual coolStep scaling value SE4:0. |
| **13** | MSTEP3 | SG3 | SE3 | Microstep counter for coil A<br>*or*<br>stallGuard2 value SG9:5 and coolStep value SE4:0 | stallGuard2 value SG9:5 and the actual coolStep scaling value SE4:0. |
| **12** | MSTEP2 | SG2 | SE2 | Microstep counter for coil A<br>*or*<br>stallGuard2 value SG9:5 and coolStep value SE4:0 | stallGuard2 value SG9:5 and the actual coolStep scaling value SE4:0. |
| **11** | MSTEP1 | SG1 | SE1 | Microstep counter for coil A<br>*or*<br>stallGuard2 value SG9:5 and coolStep value SE4:0 | stallGuard2 value SG9:5 and the actual coolStep scaling value SE4:0. |
| **10** | MSTEP0 | SG0 | SE0 | Microstep counter for coil A<br>*or*<br>stallGuard2 value SG9:5 and coolStep value SE4:0 | stallGuard2 value SG9:5 and the actual coolStep scaling value SE4:0. |
| **9** | Reserved | | | | |
| **8** | Reserved | | | | |
| **7** | STST | | | Standstill indicator | 0: No standstill condition detected.<br>1: No active edge occurred on the STEP input during the last 2²⁰ system clock cycles. |
| **6** | OLB | | | Open load indicator | 0: No open load condition detected. |
| **5** | OLA | | | Open load indicator | 0: No open load condition detected.<br>1: No chopper event has happened during the last period with constant coil polarity. Only a current above 1/16 of the maximum setting can clear this bit!<br><br>*Hint: This bit is only a status indicator. The chip takes no other action when this bit is set. False indications may occur during fast motion and at standstill. Check this bit only during slow motion.* |
| **4** | S2GB | | | Short to GND detection bits on high-side transistors | 0: No short to ground shutdown condition.<br>1: Short to ground shutdown condition. The short counter is incremented by each short circuit and the chopper cycle is suspended. The counter is decremented for each phase polarity change. The MOSFETs are shut off when the counter reaches 3 and remain shut off until the shutdown condition is cleared by disabling and re-enabling the driver. The shutdown conditions reset by deasserting the ENN input or clearing the TOFF parameter. |
| **3** | S2GA | | | Short to GND detection bits on high-side transistors | 0: No short to ground shutdown condition.<br>1: Short to ground shutdown condition. The short counter is incremented by each short circuit and the chopper cycle is suspended. The counter is decremented for each phase polarity change. The MOSFETs are shut off when the counter reaches 3 and remain shut off until the shutdown condition is cleared by disabling and re-enabling the driver. The shutdown conditions reset by deasserting the ENN input or clearing the TOFF parameter. |
| **2** | OTPW | | | Overtemperature warning | 0: No overtemperature warning condition.<br>1: Warning threshold is active. |
| **1** | OT | | | Overtemperature shutdown | 0: No overtemperature shutdown condition.<br>1: Overtemperature shutdown has occurred. |
| **0** | SG | | | stallGuard2 status | 0: No motor stall detected.<br>1: stallGuard2 threshold has been reached, and the SG_TST output is driven high. |
