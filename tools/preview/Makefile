# Previsualizador de la interfaz para el PC.
#
#   make && ./preview salida/
#
# Compila los mismos ui.c y screens.c que van a la consola. El stub de switch.h
# solo aporta los tipos y los nombres de los botones; no emula nada.

SRC_DIR  := ../../switch/source
COMMON   := ../../common/source

CFLAGS   := -O2 -g -Wall -Wextra -Wno-unused-parameter \
            -Istub -I$(SRC_DIR) -I$(COMMON) $(shell sdl2-config --cflags)
LDFLAGS  := $(shell sdl2-config --libs) -lSDL2_ttf -lSDL2_image -lSDL2_gfx -lm

OBJS := preview.o ui.o screens.o

all: preview musica

preview: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

# Vuelca la musica de fondo a WAV, con el mismo generador que la consola.
musica: musica.o audio.o
	$(CC) -o $@ musica.o audio.o $(LDFLAGS) -lSDL2_mixer

audio.o: $(SRC_DIR)/audio.c
	$(CC) $(CFLAGS) -c -o $@ $<

musica.o: musica.c
	$(CC) $(CFLAGS) -c -o $@ $<

ui.o: $(SRC_DIR)/ui.c
	$(CC) $(CFLAGS) -c -o $@ $<

screens.o: $(SRC_DIR)/screens.c
	$(CC) $(CFLAGS) -c -o $@ $<

preview.o: preview.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f preview musica $(OBJS) musica.o audio.o

.PHONY: clean all
