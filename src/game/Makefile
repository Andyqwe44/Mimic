CXX = g++
CXXFLAGS = -Wall -Wextra -pedantic -std=c++17
TARGET = main
SRCS = main.cpp config.cpp board.cpp network.cpp
OBJS = $(SRCS:.cpp=.o)

# Windows: Winsock
ifeq ($(OS),Windows_NT)
	LDLIBS = -lws2_32
	EXT = .exe
else
	LDLIBS =
	EXT =
endif

all: $(TARGET)$(EXT)

$(TARGET)$(EXT): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
ifeq ($(OS),Windows_NT)
	cmd //C "del /Q /F $(TARGET).exe $(OBJS) 2>NUL"
else
	rm -f $(TARGET) $(OBJS)
endif

run: $(TARGET)$(EXT)
	./$(TARGET)$(EXT)
