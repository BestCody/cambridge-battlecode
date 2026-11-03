#pragma once
// Engine per map print (width, height, camp groups, gems): 'a' = v119, 'b' = v130.
// Chosen from 700 games per engine against test-bot (5 fuels x 70 maps x 2 seats);
// unknown maps play the default.
struct Route {
    int width, height, groups, gems;
    char engine;
};
inline constexpr char default_engine = 'b';
inline constexpr Route routes[] = {
    {100, 100, 12, 270, 'a'},  // 00ambush v119 6/10 v130 4/10
    {37, 31, 13, 258, 'a'},  // 00anvil v119 10/10 v130 0/10
    {29, 29, 34, 154, 'a'},  // 00breadcrumbs v119 8/10 v130 6/10
    {41, 41, 20, 280, 'a'},  // 00collision v119 10/10 v130 3/10
    {71, 71, 30, 396, 'a'},  // 00corners v119 9/10 v130 8/10
    {79, 79, 11, 238, 'a'},  // 00dunes v119 5/10 v130 3/10
    {41, 30, 8, 230, 'a'},  // 00eight v119 9/10 v130 8/10
    {30, 30, 6, 68, 'a'},  // 00fragment v119 10/10 v130 5/10
    {93, 93, 9, 113, 'a'},  // 00galaxy v119 5/10 v130 2/10
    {30, 30, 19, 200, 'a'},  // 00gemfight v119 10/10 v130 5/10
    {40, 40, 14, 200, 'a'},  // 00interlock v119 6/10 v130 3/10
    {29, 29, 10, 124, 'a'},  // 00oasis v119 10/10 v130 5/10
    {45, 45, 21, 763, 'a'},  // 00quadrants v119 5/10 v130 2/10
    {30, 30, 14, 334, 'a'},  // 00reverse-spiral v119 5/10 v130 4/10
    {60, 50, 13, 160, 'a'},  // 00stampede v119 6/10 v130 6/10
    {33, 33, 9, 193, 'a'},  // 00the-line v119 1/10 v130 0/10
    {31, 31, 7, 359, 'a'},  // 00vault v119 9/10 v130 4/10
    {50, 41, 31, 1180, 'a'},  // 01boss-battle v119 4/10 v130 4/10
    {51, 40, 12, 752, 'a'},  // 01corridors v119 8/10 v130 3/10
    {50, 50, 29, 72, 'a'},  // 01elk-arena v119 7/10 v130 4/10
    {50, 40, 28, 1096, 'a'},  // 01the-farm v119 6/10 v130 0/10
    {40, 30, 28, 472, 'a'},  // 01yellow-brick-road v119 8/10 v130 6/10
    {30, 30, 14, 102, 'a'},  // 02arena v119 10/10 v130 5/10
    {60, 60, 25, 908, 'a'},  // 02cuts v119 10/10 v130 10/10
    {45, 45, 11, 404, 'a'},  // 02gift-wrapping v119 10/10 v130 5/10
    {30, 30, 15, 452, 'a'},  // 02stalactites v119 1/10 v130 1/10
    {100, 80, 33, 600, 'a'},  // 02tangle v119 7/10 v130 7/10
    {30, 30, 20, 148, 'a'},  // 02the-pond v119 10/10 v130 0/10
    {50, 46, 20, 732, 'a'},  // 02uncharted-lands v119 7/10 v130 6/10
    {51, 51, 10, 308, 'a'},  // adgato-gemcombat3 v119 10/10 v130 9/10
};
