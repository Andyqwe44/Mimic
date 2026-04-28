CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++11
TARGET = main
SRC = main.cpp

all: $(TARGET).exe

$(TARGET).exe: $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $<

ifeq ($(OS),Windows_NT)
clean:
	cmd //C "del /Q /F $(TARGET).exe 2>NUL"
else
clean:
	rm -f $(TARGET).exe
endif

run: $(TARGET).exe
	$(TARGET).exe
