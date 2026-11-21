// Handheld enclosure: ATS-Mini (ESP32 + SI4732 + 2.0" LCD) + Si5351 + PA + 20 m LPF + T/R relay + 2x18650.
// Render:  openscad -D 'part="base"' -o base.stl handheld_box.scad     (part = "base" | "lid" | "both")
//
// MEASURED from the ATS-Mini Gerber files (github.com/esp32-si4732/esp32-si4732-oshwhub, SI4732 v1.2):
//   PCB 75.6 x 30.5 mm, 4 x M2 pads (dia 2.0) 2.7 mm from each edge (70.2 x 25.1 mm spacing),
//   USB-C on the left short edge (y ~ 3.2 from the lower edge), encoder near the right end,
//   LCD bezel piece from the vendor 3D shell: 49.7 x 25.8 mm centred 29.2 mm from the left edge, 15.2 mm from the lower edge.
// DATASHEET/LISTING values: 18650 dia 18.6 x 65.5 (max), 2x18650 holder 77-78 x 40-41, Adafruit Si5351 31 x 22,
//   QRP Labs LPF kit 38.1 x 12.7, SMA panel hole 6.0 (D-flat), EC11 shaft 6, bushing M7 -> hole 7.2.
// ASSUMED (not found, check on your parts): heights of boards, PA board size, encoder/button height, PCB thickness 1.6.
$fn = 48;
part = "both";

wall = 2.2; corner = 4;
in_l = 150;          // inner X
in_w = 80;           // inner Y: battery strip (front) + radio strip (back)
base_h = 30;         // tray inner height (holder is ~22 high)
lid_h  = 12;

// --- ATS-Mini board, placed in the back strip (high Y) at the left ---
pcb_l = 75.6; pcb_w = 30.5; pcb_t = 1.6;
pcb_x = 4; pcb_y = in_w - pcb_w - 3;                 // inner coords of the PCB lower-left corner
pcb_holes = [[2.7, 2.7], [72.9, 2.7], [2.7, 27.8], [72.9, 27.8]];   // M2 pads, from PCB corner
standoff_h = 4; standoff_d = 5.2; m2_pilot = 1.7;
lcd_win = [47, 24.5]; lcd_c = [29.2, 15.2];         // LCD window size and centre, from PCB corner
enc_c = [66.0, 15.2]; enc_hole = 7.2;                // EC11 encoder: footprint centre from the Gerber silkscreen (shaft up through the lid)
usbc_y = 18.0; usbc = [10, 4];                       // USB-C centre on the left short edge (from the lower PCB edge, from the NPTH pegs)

// --- 2x18650 bay (front strip, under/left of the RF area) ---
bat_x = 4; bat_y = 3; bat_l = 78; bat_w = 41;        // holder footprint

// --- RF section (right side): Si5351 31x22, LPF 38.1x12.7, PA ~40x25 (assumed), relay, glued/taped to a plate ---
rf_x = 88;                                           // divider wall x
sma_d = 6.2;

// --- Speaker 70 x 30 x 30 (face 70 x 30 against the back wall, depth 30), under the ATS-Mini board in the back strip ---
spk = [70, 30, 30]; spk_x = 4;
amp_pos = [127, 63]; amp_size = [20, 16, 4];          // small audio amp module (PAM8403 class, assumed)

module rrect(l, w, h, r) { hull() for (x=[r, l-r], y=[r, w-r]) translate([x, y, 0]) cylinder(r=r, h=h); }
module shell(h) {
  difference() {
    rrect(in_l + 2*wall, in_w + 2*wall, h + wall, corner);
    translate([wall, wall, wall]) rrect(in_l, in_w, h + 1, corner - 1);
  }
}
boss_pos = [[6,6],[in_l-6,6],[6,in_w-6],[in_l-6,in_w-6]];
function ip(p) = [wall + p[0], wall + p[1]];        // inner -> outer coords

// ---- Module mounts: 4 posts per module (M2 pilot holes, hole positions ASSUMED 2.5 mm inside each corner) ----
post_h = 4; post_d = 4.4;
mods = [                                  // [x, y, w, h] in inner coords
  [92, 52, 31, 22],     // Si5351
  [92, 34, 38.1, 12.7], // 20 m LPF
  [96, 5, 40, 25],      // RF PA
  [132, 52, 15, 10],    // T/R relay
  [127, 63, 20, 16],    // audio amp (optional, ATS-Mini has its own)
];
module mount_posts() {
  for (m = mods) for (dx=[2.5, m[2]-2.5], dy=[2.5, m[3]-2.5])
    translate([wall+m[0]+dx, wall+m[1]+dy, wall]) difference() {
      cylinder(d=post_d, h=post_h);
      translate([0,0,1]) cylinder(d=1.7, h=post_h);
    }
}
// Cable-tie slots (3 x 1.5 mm pairs, 6 mm apart): the harness is strapped to the floor next to the modules
module tie_slots() {
  for (p = [[76,24],[76,48],[96,48],[140,30],[76,66],[40,50]])
    for (d=[0,6]) translate([wall+p[0]+d, wall+p[1], -1]) cube([1.5, 3, wall+2]);
}

module base() {
  difference() {
    union() {
      shell(base_h);
      for (p = boss_pos) translate([ip(p)[0], ip(p)[1], wall]) cylinder(d=7, h=base_h);
      mount_posts();
      // RF divider with a 14 mm gap at the floor for coax/power wires
      difference() {
        translate([wall+rf_x, wall, wall]) cube([1.6, in_w, base_h]);
        translate([wall+rf_x-1, wall+in_w/2-7, wall]) cube([4, 14, 6]);
      }
      // speaker cradle: two end blocks hold the 70 x 30 x 30 speaker against the back wall
      for (x=[spk_x-2.2, spk_x+spk[0]]) translate([wall+x, wall+in_w-spk[1], wall]) cube([2.2, spk[1], 12]);
      // battery bay retaining walls (open on top, cells held by the holder)
      difference() {
        translate([wall+bat_x-1.5, wall+bat_y-1.5, wall]) cube([bat_l+3, bat_w+3, 10]);
        translate([wall+bat_x, wall+bat_y, wall-1]) cube([bat_l, bat_w, 12]);
      }
    }
    for (p = boss_pos) translate([ip(p)[0], ip(p)[1], base_h-8]) cylinder(d=2.6, h=20);
    tie_slots();
    // harness notch across the top of the divider (wires to the board and speaker pass here)
    translate([wall+rf_x-1, wall+in_w-24, wall+base_h-7]) cube([4, 16, 8]);
    // SMA on the right end wall
    translate([in_l+wall-1, wall+in_w/2, wall+base_h/2]) rotate([0,90,0]) cylinder(d=sma_d, h=wall+2);
    // power switch, front wall
    translate([wall+100, -1, wall+base_h-8]) rotate([-90,0,0]) cylinder(d=6.2, h=wall+2);
    // speaker grille: 7 slots in the back wall in front of the speaker face
    for (i=[0:6]) translate([wall+spk_x+6+i*9, in_w+wall-1, wall+spk[2]/2-9]) cube([4, wall+2, 18]);
    // PA vents on the back wall (RF side)
    for (i=[0:4]) translate([wall+rf_x+10+i*10, in_w+wall-1, wall+6]) cube([3, wall+2, 12]);
  }
}

// The lid is printed face-down; when assembled it is flipped about the X axis, so its Y is mirrored (ly).
function ly(y) = in_w - y;
standoff_gap = 6.5; // PCB top face to lid floor: EC11 body height above the PCB (LCD sits a little below the floor)
module lid() {
  difference() {
    union() {
      shell(lid_h);
      for (p = boss_pos) translate([ip(p)[0], ip(p)[1], wall]) cylinder(d=7, h=lid_h);
      // M2 standoffs that carry the ATS-Mini board from the lid floor
      for (h = pcb_holes) translate([wall+pcb_x+h[0], wall+ly(pcb_y+h[1]), wall]) cylinder(d=standoff_d, h=standoff_gap);
    }
    for (p = boss_pos) translate([ip(p)[0], ip(p)[1], -1]) { cylinder(d=3.4, h=wall+lid_h+2); cylinder(d1=6.4, d2=3.4, h=2); }
    for (h = pcb_holes) translate([wall+pcb_x+h[0], wall+ly(pcb_y+h[1]), wall+0.5]) cylinder(d=m2_pilot, h=standoff_gap+1);
    // LCD window and encoder hole in the floor
    translate([wall+pcb_x+lcd_c[0]-lcd_win[0]/2, wall+ly(pcb_y+lcd_c[1])-lcd_win[1]/2, -1]) cube([lcd_win[0], lcd_win[1], wall+2]);
    translate([wall+pcb_x+enc_c[0], wall+ly(pcb_y+enc_c[1]), -1]) cylinder(d=enc_hole, h=wall+2);
    // USB-C slot in the left lid wall, level with the PCB
    translate([-1, wall+ly(pcb_y+usbc_y)-usbc[0]/2, wall+standoff_gap+pcb_t-1]) cube([wall+2, usbc[0], usbc[1]]);
  }
}

// ---- Ghost components for the assembly view (sizes: see header; heights partly assumed) ----
module comp(c, p, s) color(c) translate(p) cube(s);
module parts() {
  top = wall + base_h;                          // rim plane between base and lid
  pcb_z = top + lid_h - standoff_gap - pcb_t;   // PCB underside, hanging from the lid
  // ATS-Mini PCB + LCD + encoder
  comp("seagreen", [wall+pcb_x, wall+pcb_y, pcb_z], [pcb_l, pcb_w, pcb_t]);
  comp("black", [wall+pcb_x+lcd_c[0]-24.85, wall+pcb_y+lcd_c[1]-12.9, pcb_z+pcb_t], [49.7, 25.8, 2.5]);
  color("dimgray") translate([wall+pcb_x+enc_c[0], wall+pcb_y+enc_c[1], pcb_z+pcb_t]) cylinder(d=7, h=standoff_gap+wall+1.5);
  comp("silver", [wall+pcb_x-2, wall+pcb_y+usbc_y-4.5, pcb_z-3], [8, 9, 3.2]);          // USB-C
  // 2 x 18650 in the holder
  comp("dimgray", [wall+bat_x, wall+bat_y, wall], [bat_l, bat_w, 3]);
  for (i=[0,1]) color("goldenrod") translate([wall+bat_x+6, wall+bat_y+10.2+i*20.6, wall+3+9.3]) rotate([0,90,0]) cylinder(d=18.6, h=65.5);
  // RF section: Si5351, LPF, PA with heatsink, relay, SMA
  comp("royalblue", [wall+92, wall+52, wall+post_h], [31, 22, 4]);
  comp("darkorange", [wall+92, wall+34, wall+post_h], [38.1, 12.7, 9]);
  comp("firebrick", [wall+96, wall+5, wall+post_h], [40, 25, 6]);
  comp("slategray", [wall+96, wall+5, wall+post_h+6], [40, 25, 12]);
  comp("white", [wall+132, wall+52, wall+post_h], [15, 10, 12]);
  // speaker and audio amp
  color("darkslategray") translate([wall+spk_x, wall+in_w-spk[1], wall]) cube(spk);
  comp("purple", [wall+amp_pos[0], wall+amp_pos[1], wall+post_h], amp_size);
  wires();
  color("gold") translate([in_l+wall-4, wall+in_w/2, wall+base_h/2]) rotate([0,90,0]) cylinder(d=6, h=8);
}

// ---- Wiring harness (ghost): coloured wires along their routed paths, points in INNER coords [x, y, z-above-floor] ----
module wire(pts, c, d=1.8) color(c) for (i=[0:len(pts)-2])
  hull() { translate([wall+pts[i][0],   wall+pts[i][1],   wall+pts[i][2]])   sphere(d=d, $fn=8);
           translate([wall+pts[i+1][0], wall+pts[i+1][1], wall+pts[i+1][2]]) sphere(d=d, $fn=8); }
module wires() {
  bz = base_h + lid_h - standoff_gap - pcb_t - 1;   // board underside height above the floor (inner coords)
  // battery -> floor gap in the divider -> switch (front wall) -> top notch -> ATS-Mini BAT
  wire([[82,30,8],[86,40,3],[90,40,3],[90,12,3],[100,6,22]], "red");
  wire([[82,34,8],[86,42,2],[91,42,2],[91,14,2],[98,6,20]], "black");
  wire([[100,6,24],[90,6,26],[90,64,26],[84,64,27],[76,64,bz-1]], "red");
  // Si5351 I2C/power -> notch -> board GPIO13/14
  wire([[94,60,10],[90,62,24],[84,62,27],[70,60,bz-1]], "gold");
  wire([[96,60,10],[90,66,24],[84,66,27],[68,64,bz-1]], "yellow");
  // CLK -> PA -> LPF -> relay (coax, orange), relay COM -> SMA
  wire([[100,52,8],[90,52,6],[90,17,6],[98,17,8]], "darkorange", 2.4);
  wire([[130,30,10],[130,34,12]], "darkorange", 2.4);
  wire([[128,46,12],[139,50,12],[139,52,12]], "darkorange", 2.4);
  wire([[147,57,10],[150,50,12],[152,40,15]], "darkorange", 2.4);
  // relay NC -> SI4732 AMI (shielded, via notch); relay coil <- GPIO26
  wire([[134,62,12],[100,68,26],[84,68,27],[66,66,bz-1]], "silver", 1.6);
  wire([[140,62,12],[110,70,27],[84,70,27],[62,66,bz-1]], "limegreen", 1.4);
  // --- the rest of the harness ---
  wire([[96,64,10],[90,63,25],[84,63,27],[72,62,bz-1]], "orangered", 1.4);     // 3V3 -> Si5351 VIN
  wire([[98,64,10],[90,65,25],[84,65,27],[74,64,bz-1]], "dimgray", 1.4);       // GND -> Si5351
  wire([[136,64,12],[110,66,26],[84,64,27],[72,66,bz-1]], "orangered", 1.4);   // 3V3 -> relay driver
  wire([[138,64,12],[112,68,26],[84,67,27],[70,67,bz-1]], "dimgray", 1.4);     // GND -> relay driver
  wire([[100,22,10],[92,22,6],[90,22,6]], "red", 1.8);                          // PA supply tap (after the switch)
  wire([[104,22,10],[92,26,4],[90,26,4]], "black", 1.8);                        // PA GND
  wire([[108,26,10],[104,20,12],[92,20,12],[90,20,12],[90,58,12],[88,60,26],[78,60,bz-1]], "deeppink", 1.4); // PA enable GPIO12(?)
  wire([[128,50,12],[122,44,12],[116,44,12]], "darkorange", 2.4);               // LPF jumper to relay path
  wire([[120,62,12],[120,70,12],[130,70,12],[136,64,12]], "dimgray", 1.2);     // audio amp (optional) GND tie
  wire([[127,70,8],[100,72,26],[84,72,27],[56,66,bz-1]], "dodgerblue", 1.4);   // audio amp in (optional)
  // speaker -> board speaker pads
  wire([[60,56,spk[2]-1],[58,56,bz-1]], "red", 1.6);
  wire([[64,56,spk[2]-1],[62,58,bz-1]], "black", 1.6);
}

if (part == "base") base();
else if (part == "lid") lid();
else if (part == "open") { base(); parts(); }
else if (part == "assembly" || part == "exploded") {
  lift = (part == "exploded") ? 45 : 0;
  base(); parts();
  translate([0, in_w + 2*wall, wall + base_h + lid_h + wall + lift]) rotate([180,0,0]) color("lightsteelblue", 0.55) lid();
}
else { base(); translate([0, in_w + 2*wall + 10, 0]) lid(); }
