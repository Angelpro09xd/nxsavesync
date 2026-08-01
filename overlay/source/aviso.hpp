#pragma once

// Aviso sobre el menu HOME.
//
// Es una capa propia, aparte de la de Tesla, con su tamano y su posicion. Se
// hace asi y no reaprovechando la de Tesla por dos motivos: la de Tesla es
// estrecha y va pegada a la derecha, y ademas se dibuja con sus coordenadas.
// Una capa propia cae exactamente donde el usuario la quiera sin tocar nada del
// panel del combo.
//
// Lo importante: dibujar y quitarle el mando al menu HOME son cosas separadas.
// Esta capa nunca pide el primer plano (`requestForeground`), asi que se ve
// encima del menu sin que dejes de manejarlo.

// Dibuja un fotograma del aviso. Se llama desde el bucle de espera del combo,
// asi que solo corre cuando el panel NO esta abierto. Se inicializa sola la
// primera vez.
extern "C" void nxss_aviso_tick(void);

// Suelta la capa. Se llama al cerrar el overlay.
extern "C" void nxss_aviso_exit(void);
