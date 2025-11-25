CC=gcc
CFLAGS=-Wall -Wextra -std=c99

# Lista de todos os executáveis
TARGETS=controlador cliente veiculo

all: $(TARGETS)

# Regra genérica para criar um executável a partir de um ficheiro .c
# $@ é o nome do alvo (ex: 'controlador')
# $< é o nome da primeira dependência (ex: 'controlador.c')
controlador: controlador.c
	$(CC) $(CFLAGS) -o $@ $<

cliente: cliente.c
	$(CC) $(CFLAGS) -o $@ $<

veiculo: veiculo.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS) *.o
