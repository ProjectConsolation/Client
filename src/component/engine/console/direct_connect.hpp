#pragma once
namespace direct_connect
{
    // Called before forwarding map/exec startup commands. False means the
    // native dedicated transition needs one frame (or startup was refused).
    bool prepare_dedicated_launch();
}
