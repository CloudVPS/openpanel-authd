include makeinclude

OBJ	= main.o version.o

all: authd.exe
	grace mkapp authd

version.cpp:
	grace mkversion version.cpp

authd.exe: $(OBJ)
	$(LD) $(LDFLAGS) -o authd.exe $(OBJ) $(LIBS)

clean:
	rm -f *.o *.exe
	rm -rf authd.app
	rm -f authd

SUFFIXES: .cpp .o
.cpp.o:
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $<
