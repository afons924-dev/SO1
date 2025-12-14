#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

#define FIFO_PRINCIPAL "controlador_fifo"
#define FIFO_CLIENTE_TEMPLATE "cliente_%s_fifo"

char fifo_cliente_nome[100];
int fd_fifo_cliente = -1;

// Função de limpeza chamada à saída para remover o FIFO
void cleanup_cliente() {
    if (fifo_cliente_nome[0] != '\0') {
        // Tenta remover. Se falhar, é porque já foi removido ou não existe.
        unlink(fifo_cliente_nome);
    }
}

// Handler para o sinal SIGINT (Ctrl+C)
void handle_sigint_cliente(int sig) {
    (void)sig;
    exit(0);
}

// Handler para SIGCHLD (quando o filho morre/termina)
void handle_sigchld(int sig) {
    (void)sig;
    // Se o filho terminou, o servidor fechou a conexão ou houve erro.
    // O pai deve terminar também.
    // Usamos _exit para ser seguro dentro de um handler,
    // mas isso salta a limpeza do atexit no pai.
    // Como o filho também chama atexit/limpeza, o FIFO deve ser removido pelo filho.
    _exit(0);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Uso: %s <username>\n", argv[0]);
        return 1;
    }

    char *username = argv[1];
    int fd_fifo_principal;
    pid_t pid;

    printf("Cliente '%s' a iniciar...\n", username);

    fd_fifo_principal = open(FIFO_PRINCIPAL, O_WRONLY);
    if (fd_fifo_principal == -1) {
        perror("Erro ao abrir FIFO do controlador. O controlador está a correr?");
        return 1;
    }
    printf("Ligado ao controlador.\n");

    snprintf(fifo_cliente_nome, sizeof(fifo_cliente_nome), FIFO_CLIENTE_TEMPLATE, username);
    atexit(cleanup_cliente);
    signal(SIGINT, handle_sigint_cliente);

    // Tenta criar o FIFO. Se já existir, remove e recria.
    // Nota: Em um sistema real, devíamos verificar se o FIFO pertence a um processo vivo.
    // Aqui assumimos que se estamos a iniciar, queremos limpar o anterior.
    // Mas se houver outro cliente a correr, vamos "roubar" o FIFO,
    // mas o login vai falhar no servidor se já estiver logado.
    unlink(fifo_cliente_nome);
    if (mkfifo(fifo_cliente_nome, 0666) == -1) {
        perror("Erro ao criar o FIFO do cliente");
        return 1;
    }
    printf("FIFO do cliente '%s' criado.\n", fifo_cliente_nome);

    // Enviar LOGIN
    char mensagem_login[256];
    snprintf(mensagem_login, sizeof(mensagem_login), "LOGIN %s %s", username, fifo_cliente_nome);
    write(fd_fifo_principal, mensagem_login, strlen(mensagem_login));
    printf("Mensagem de login enviada. A aguardar resposta...\n");
    fflush(stdout);

    // Abrir FIFO para ler a resposta do Login
    fd_fifo_cliente = open(fifo_cliente_nome, O_RDONLY);
    if (fd_fifo_cliente == -1) {
        perror("Erro ao abrir FIFO do cliente para leitura");
        return 1;
    }

    char buffer_resp[512];
    int n = read(fd_fifo_cliente, buffer_resp, sizeof(buffer_resp)-1);
    if (n <= 0) {
        printf("Erro: Não foi possível receber resposta do servidor.\n");
        return 1;
    }
    buffer_resp[n] = '\0';
    printf("Servidor: %s\n", buffer_resp);
    fflush(stdout);

    if (strncmp(buffer_resp, "Erro", 4) == 0) {
        // Login falhou
        return 1;
    }

    // Login com sucesso. Iniciar processo de escuta.
    pid = fork();
    if (pid == -1) {
        perror("Erro no fork");
        return 1;
    }

    if (pid == 0) { // Filho: Recebe mensagens
        // O filho herda fd_fifo_cliente aberto.
        signal(SIGTERM, handle_sigint_cliente);

        printf("\n[INFO] Estou à escuta de mensagens.\n");
        char buffer_recebido[512];
        while ((n = read(fd_fifo_cliente, buffer_recebido, sizeof(buffer_recebido)-1)) > 0) {
            buffer_recebido[n] = '\0';
            printf("\n[MENSAGEM] %s\n> ", buffer_recebido);
            fflush(stdout);
        }
        // Se read retornar 0, o servidor fechou o FIFO (Logout ou Shutdown)
        printf("\nSessão terminada pelo servidor.\n");
        close(fd_fifo_cliente);
        exit(0);
    } else { // Pai: Envia comandos
        // O pai não precisa ler do FIFO do cliente
        close(fd_fifo_cliente);

        // Se o filho morrer (ex: servidor fechou conexão), o pai deve sair
        signal(SIGCHLD, handle_sigchld);

        char comando[256];
        printf("Introduza os seus comandos (ex: 'agendar 100 lisboa 50', 'terminar').\n");
        while (1) {
            printf("> ");
            if (fgets(comando, sizeof(comando), stdin) == NULL) break;
            comando[strcspn(comando, "\n")] = 0;

            if (strcmp(comando, "help") == 0) {
                printf("--- Comandos do Cliente ---\n");
                printf("  agendar <hora> <local> <distancia> - Agenda um servico.\n");
                printf("  consultar                      - Mostra os seus servicos agendados.\n");
                printf("  cancelar <id>                  - Cancela um servico agendado (se id for 0, cancela todos).\n");
                printf("  terminar                       - Sai da aplicacao cliente.\n");
                printf("  help                           - Mostra esta ajuda.\n");
            } else if (strcmp(comando, "terminar") == 0) {
                char msg_terminar[300];
                snprintf(msg_terminar, sizeof(msg_terminar), "%s terminar", username);
                write(fd_fifo_principal, msg_terminar, strlen(msg_terminar));
                // NÃO fazemos break aqui. Esperamos que o servidor feche a conexão.
                // Se o servidor recusar (em viagem), receberemos mensagem de erro pelo filho.
            } else {
                if (strncmp(comando, "agendar", 7) == 0 || strcmp(comando, "consultar") == 0 || strncmp(comando, "cancelar", 8) == 0) {
                    char mensagem_comando[512];
                    snprintf(mensagem_comando, sizeof(mensagem_comando), "%s %s", username, comando);
                    write(fd_fifo_principal, mensagem_comando, strlen(mensagem_comando));
                } else {
                    printf("Comando desconhecido. Use 'help' para ver a lista de comandos.\n");
                }
            }
        }
        kill(pid, SIGTERM);
        wait(NULL);
        close(fd_fifo_principal);
        printf("A terminar a sessão...\n");
    }
    return 0;
}
