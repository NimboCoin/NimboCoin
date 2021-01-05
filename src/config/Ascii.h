// Copyright (c) 2018, The TurtleCoin Developers
// Copyright (c) 2019, The DeroGold Association
//
// Please see the included LICENSE file for more information

#include <string>
#include <sstream>
#include <iostream>
#pragma once

const std::string windowsAsciiArt =

"\n              //  888b    888 8888888 888b     d888 888888b.    .d88888b.         \n"
"              //  8888b   888   888   8888b   d8888 888   88b  d88P  Y88b         \n"
"              //  88888b  888   888   88888b.d88888 888  .88P  888     888        \n"
"              //  888Y88b 888   888   888Y88888P888 8888888K.  888     888        \n"
"              //  888 Y88b888   888   888 Y888P 888 888   Y88b 888     888        \n"
"              //  888  Y88888   888   888  Y8P  888 888    888 888     888        \n"
"              //  888   Y8888   888   888       888 888   d88P Y88b. .d88P        \n"
"              //  888    Y888 8888888 888       888 8888888P     Y88888P          \n";






const std::string nonWindowsAsciiArt =
"\n                 ███╗   ██╗██╗███╗   ███╗██████╗  ██████╗            \n"
"                 ████╗  ██║██║████╗ ████║██╔══██╗██╔═══██╗           \n"
"                 ██╔██╗ ██║██║██╔████╔██║██████╔╝██║   ██║           \n"
"                 ██║╚██╗██║██║██║╚██╔╝██║██╔══██╗██║   ██║           \n"
"                 ██║ ╚████║██║██║ ╚═╝ ██║██████╔╝╚██████╔╝           \n"
"                 ╚═╝  ╚═══╝╚═╝╚═╝     ╚═╝╚═════╝  ╚═════╝            \n";




/* Windows has some characters it won't display in a terminal. If your ascii
   art works fine on Windows and Linux terminals, just replace 'asciiArt' with
   the art itself, and remove these two #ifdefs and above ascii arts */
#ifdef _WIN32

const std::string asciiArt = windowsAsciiArt;

#else
const std::string asciiArt = nonWindowsAsciiArt;
#endif
