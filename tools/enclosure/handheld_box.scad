// Handheld enclosure for the ESP32 + SI4732 radio with Si5351 TX, PA, 20 m LPF, T/R relay and 2x18650.
// ALL DIMENSIONS BELOW ARE ASSUMED (not measured). Measure your boards and edit the "Parts" block first.
// Render:  openscad -D 'part="base"' -o base.stl handheld_box.scad     (part = "base" | "lid" | "both")
$fn = 48;
part = "both";

// ---- Outer shell ----
wall   = 2.2;
in_l   = 150;   // inner length (X): RX board on the left, RF section on the right
in_w   = 72;    // inner width (Y)
base_h = 24;    // inner height of the tray
lid_h  = 9;     // inner height of the lid
corner = 4;

// ---- Parts (assumed sizes, mm) ----
radio_l = 70; radio_w = 40;         // ESP32+SI4732+LCD board footprint
lcd_win = [50, 28];                 // LCD window in the lid
lcd_pos = [30, in_w/2];             // window centre (x, y) from inner corner, lid
enc_pos = [62, in_w/2 + 14]; enc_d = 9;          // rotary encoder hole
btn_pos = [[62, in_w/2 - 2], [62, in_w/2 - 14]]; btn_d = 7;
rf_x    = 84;                       // divider wall x (RF section to the right of it)
standoff_h = 6; standoff_d = 6; hole_d = 2.5;    // M2.5 standoffs
radio_hole_pos = [[8,16],[8,56],[70,16],[70,56]];// radio board mounting holes (inner coords)
rf_hole_pos    = [[96,14],[96,58],[140,14],[140,58]]; // RF modules plate holes
bat_l = 70; bat_w = 38; bat_h = 19;              // 2x18650 side by side pocket
sma_d = 6.5; usbc = [10, 4];

module rrect(l, w, h, r) {
  hull() for (x=[r, l-r], y=[r, w-r]) translate([x, y, 0]) cylinder(r=r, h=h);
}

module shell(h) {
  difference() {
    rrect(in_l + 2*wall, in_w + 2*wall, h + wall, corner);
    translate([wall, wall, wall]) rrect(in_l, in_w, h + 1, corner - 1);
  }
}

boss_pos = [[6,6],[in_l-6,6],[6,in_w-6],[in_l-6,in_w-6]];

module base() {
  difference() {
    union() {
      shell(base_h);
      // screw bosses
      for (p = boss_pos) translate([wall+p[0], wall+p[1], wall]) cylinder(d=7, h=base_h);
      // standoffs
      for (p = radio_hole_pos) translate([wall+p[0], wall+p[1], wall]) cylinder(d=standoff_d, h=standoff_h);
      for (p = rf_hole_pos)    translate([wall+p[0], wall+p[1], wall]) cylinder(d=standoff_d, h=standoff_h);
      // RF shield divider with a 14 mm gap at the bottom for coax / power wires
      difference() {
        translate([wall+rf_x, wall, wall]) cube([1.6, in_w, base_h]);
        translate([wall+rf_x-1, wall+in_w/2-7, wall]) cube([4, 14, 6]);
      }
      // battery pocket walls (under the radio board, left-bottom)
    }
    // pilot holes in bosses and standoffs
    for (p = boss_pos) translate([wall+p[0], wall+p[1], base_h-8]) cylinder(d=2.6, h=20);
    for (p = concat(radio_hole_pos, rf_hole_pos)) translate([wall+p[0], wall+p[1], wall+1]) cylinder(d=hole_d, h=standoff_h+1);
    // SMA antenna on the right end wall
    translate([in_l+wall-1, wall+in_w/2, wall+base_h/2]) rotate([0,90,0]) cylinder(d=sma_d, h=wall+2);
    // USB-C on the left end wall
    translate([-1, wall+in_w/2-usbc[0]/2, wall+6]) cube([wall+2, usbc[0], usbc[1]]);
    // power switch on the front wall
    translate([wall+20, -1, wall+base_h-8]) rotate([-90,0,0]) cylinder(d=6.2, h=wall+2);
    // vents for the PA on the back wall
    for (i=[0:4]) translate([wall+100+i*8, in_w+wall-1, wall+6]) cube([3, wall+2, 12]);
  }
}

module lid() {
  difference() {
    union() {
      shell(lid_h);
      for (p = boss_pos) translate([wall+p[0], wall+p[1], wall]) cylinder(d=7, h=lid_h);
    }
    // screw clearance + countersink from outside
    for (p = boss_pos) translate([wall+p[0], wall+p[1], -1]) {
      cylinder(d=3.4, h=wall+lid_h+2);
      cylinder(d1=6.4, d2=3.4, h=2);
    }
    // openings (the lid is printed top-down, so the window is in its floor)
    translate([wall+lcd_pos[0]-lcd_win[0]/2, wall+lcd_pos[1]-lcd_win[1]/2, -1]) cube([lcd_win[0], lcd_win[1], wall+2]);
    translate([wall+enc_pos[0], wall+enc_pos[1], -1]) cylinder(d=enc_d, h=wall+2);
    for (b = btn_pos) translate([wall+b[0], wall+b[1], -1]) cylinder(d=btn_d, h=wall+2);
  }
}

if (part == "base") base();
else if (part == "lid") lid();
else { base(); translate([0, in_w + 2*wall + 10, 0]) lid(); }
