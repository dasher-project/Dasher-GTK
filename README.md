# The Dasher Text Entry System ✨
Dasher is a zooming predictive text entry system, designed for situations
where keyboard input is impractical (for instance, accessibility or PDAs). It
is usable with highly limited amounts of physical input while still allowing
high rates of text entry.

## Dasher GTK 🖥️
Based on the DasherCore library this repository aims at implementing a fully featured new version of Dasher. Due to the usage of [GTK4](https://gtk.org/) as a frontend we strive to develop a multi-platform application that can be used regardless of the computing hardware. This project is still in its early stages of development and will need some time to be able to replace the Dasher 5 version.

## Build Instructions ⚙️🏗️
This library version can be build, simply by generating the required make files via CMake and then building with these. A viable workflow could look something like this:

1. Make sure to install all required dependencies for building on your platform. See section below.
2. Clone the repository with all submodules: `git clone --recursive https://github.com/dasher-project/Dasher-GTK.git ./DasherGTK`
3. Generate some project files with CMake:
  * `cd ./DasherGTK && mkdir build && cd build`
  * `cmake ..`
4. Build the project with the selected build system (e.g. Visual Studio on Windows or `make` on Linux and MacOS). If you are on Windows and use the pre-build GTK binaries be sure to select an optimized release build or it will not be binary compatible with the library. 

### Build Dependencies 📦
* Windows: You will need to install GTK. The easiest way to install it is to grab a pre-build copy from [GVSBuild](https://github.com/wingtk/gvsbuild/releases) and extract it to `C:\gtk`. Afterwards add `C:\gtk\bin` to your path. Additionally, you will need basic C++ development tools like CMake, Git and a compiler like Clang or MSVC (from VisualStudio).
* Linux: You will need to install GTK4 and GTK4mm using your package manager + some essential buildtools. E.g. using `apt-get install build-essential libgtk-4-dev libgtkmm-4.0-dev`
* MacOS: You will also need to install GTK4 and GTK4mm using your package manager + some essential buildtools. E.g. using `brew install gtk4 gtkmm4`

## License 📎

As this front-end is based on the DasherCore and we hope to attract some help from other developers and encurage everyone to extend the Dasher system, we licensed this front-end also under the MIT license.

## Support and Feedback 🗣️

Please file any bug reports in the issues of this repository. If you want to help and join the development group, either send us a pull request or get in contact using [Slack in the OpenAAC group](https://join.slack.com/t/openaac/shared_invite/enQtNTQwNDgwODYyNjU5LTAwODNmZjM4ZmJmOTJkYTY2MWZkNjc0MDQ0NTcwMTRmMzY0MWI3OWJiNGYwZGIzMzc2YTk2N2FiY2JlYTI5Njc).

You can find the Dasher website and more info at:
https://github.com/dasher-project
