
#include <arpa/inet.h>        // htons
#include <linux/if_ether.h>   // struct ethhdr
#include <linux/if_packet.h>  // struct sockaddr_ll
#include <linux/ip.h>         // struct iphdr
#include <stdbool.h>          // boolean type

#include <stdio.h>  // provisorio

#include "m_error.h"
#include "network.h"
#include "timer.h"

// bit dont fragment flag do cabeçalho IP
#define IP_DF 0x4000

// bit more fragments do cabeçalho IP
#define IP_MF 0x2000

// mascara para testar o offset do fragmento
#define IP_OFFMASK 0x1FFF

// máximo de pacotes IP que podem estar fragmentados simultaneamente,
// os pacotes que chegarem alem desse limite não serão calculados
// e um erro informado
#define MAX_REASSEMBLIES 16

// maximo de fragmentos que um pacote IP pode ter,
// acima disso um erro sera emitido, e os demais fragmentos desse pacote
// não serão computados
#define MAX_FRAGMENTS 32

// tempo de vida maximo de um pacote fragmentado em segundos,
// cada pacote possui um contador unico, independente da atualização
// do programa
#define LIFETIME_FRAG 1.0

// numero maximo de fragmentos de um pacote aitigido
#define ER_MAX_FRAGMENTS -2

// atualiza o total de pacotes fragmentados
#define DEC_REASSEMBLE( var ) ( ( var > 0 ) ? ( var )-- : ( var = 0 ) )

// defined in main
extern bool udp;
extern uint8_t tic_tac;

// Aproveitamos do fato dos cabeçalhos TCP e UDP
// receberem as portas de origem e destino na mesma ordem,
// e como atributos iniciais, assim podemos utilizar esse estrutura
// simplificada para extrair as portas tanto de pacotes
// TCP quanto UDP, lembrando que não utilizaremos outros campos
// dos cabeçalhos.
struct tcp_udp_h
{
  uint16_t source;
  uint16_t dest;
};

// utilzado para identificar a camada de transporte (TCP, UDP)
// dos fragmentos de um pacote e tambem ter controle de tempo de vida
// do pacote e numero de fragmentos do pacote
struct pkt_ip_fragment
{
  uint16_t pkt_id;       // IP header ID value
  uint16_t source_port;  // IP header source port value
  uint16_t dest_port;    // IP header dest port value
  uint8_t c_frag;        // count of fragments, limit is MAX_FRAGMENTS
  float ttl;             // lifetime of packet
};

static int
is_first_frag ( struct iphdr *l3, struct tcp_udp_h *l4 );
static int
is_frag ( struct iphdr *l3 );

static void
insert_data_packet ( struct packet *pkt,
                     const uint8_t direction,
                     const uint32_t local_address,
                     const uint32_t remote_address,
                     const uint16_t local_port,
                     const uint16_t remote_port );

static void
clear_frag ();

// armazena os dados da camada de transporte dos pacotes fragmentados
static struct pkt_ip_fragment pkt_ip_frag[MAX_REASSEMBLIES] = {0};

// contador de pacotes IP que estão fragmentados
static uint8_t count_reassemblies;

int
parse_packet ( struct packet *pkt, unsigned char *buf, struct sockaddr_ll *ll )
{
  struct ethhdr *l2;
  struct iphdr *l3;
  struct tcp_udp_h *l4;

  l2 = ( struct ethhdr * ) buf;

  // not is a packet internet protocol
  if ( ntohs ( l2->h_proto ) != ETH_P_IP )
    goto END;

  l3 = ( struct iphdr * ) ( buf + ETH_HLEN );

  // caso tenha farejado pacotes TCP e opção udp nao estaja ligada. Default
  if ( l3->protocol == IPPROTO_TCP && !udp )
    l4 = ( struct tcp_udp_h * ) ( buf + ETH_HLEN + ( l3->ihl * 4 ) );
  // caso tenha farejado pacote UDP e a opção udp esteja ligada
  else if ( l3->protocol == IPPROTO_UDP && udp )
    l4 = ( struct tcp_udp_h * ) ( buf + ETH_HLEN + ( l3->ihl * 4 ) );
  // pacote não suportado
  else
    goto END;

  // printf("TOTAL count_reassemblies - %d\n", count_reassemblies);
  // atigido MAX_REASSEMBLIES, dados não computados
  if ( is_first_frag ( l3, l4 ) == -1 )
    goto END;

  int id = is_frag ( l3 );
  // create packet
  if ( ll->sll_pkttype == PACKET_OUTGOING )
    {  // upload
      if ( id == -1 )
        // não é um fragmento, assumi que isso é maioria dos casos
        insert_data_packet ( pkt,
                             PKT_UPL,
                             l3->saddr,
                             l3->daddr,
                             ntohs ( l4->source ),
                             ntohs ( l4->dest ) );

      else if ( id >= 0 )
        // é um fragmento, pega dados da camada de transporte
        // no array de pacotes fragmentados
        insert_data_packet ( pkt,
                             PKT_UPL,
                             l3->saddr,
                             l3->daddr,
                             pkt_ip_frag[id].source_port,
                             pkt_ip_frag[id].dest_port );
      else
        // é um fragmento, porem maximo de fragmentos de um pacote atingido.
        // dados não serão computados
        goto END;

      return 1;
    }
  else
    {  // download
      if ( id == -1 )
        // não é um fragmento, assumi que isso é maioria dos casos
        insert_data_packet ( pkt,
                             PKT_DOWN,
                             l3->daddr,
                             l3->saddr,
                             ntohs ( l4->dest ),
                             ntohs ( l4->source ) );
      else if ( id >= 0 )
        // é um fragmento, pega dados da camada de transporte
        // no array de pacotes fragmentados
        insert_data_packet ( pkt,
                             PKT_DOWN,
                             l3->daddr,
                             l3->saddr,
                             pkt_ip_frag[id].dest_port,
                             pkt_ip_frag[id].source_port );
      else
        // é um fragmento, porem maximo de fragmentos de um pacote atingido.
        // dados não serão computados
        goto END;

      return 1;
    }

// caso exista pacotes que foram fragmentados,
// faz checagem e descarta pacotes que ainda não enviaram todos
// os fragmentos no tempo limite de LIFETIME_FRAG segundos
END:
  if ( count_reassemblies )
    clear_frag ();

  return 0;
}

// Testa se o bit more fragments (MF) esta ligado
// e se o offset é 0, caso sim esse é o primeiro fragmento,
// então ocupa um espaço na estrutura pkt_ip_frag com os dados
// da camada de transporte do pacote, para associar aos demais
// fragmentos posteriormente.
// retorna 1 para primeiro fragmento
// retorna 0 se não for primeiro fragmento
// retorna -1 caso de erro, buffer cheio
static int
is_first_frag ( struct iphdr *l3, struct tcp_udp_h *l4 )
{
  // bit não fragmente ligado, logo não pode ser um fragmento
  if ( ntohs ( l3->frag_off ) & IP_DF )
    return 0;

  // bit MF ligado e offset igual a 0,
  // indica que é o primeiro fragmento
  if ( ( ntohs ( l3->frag_off ) & IP_MF ) &&
       ( ( ntohs ( l3->frag_off ) & IP_OFFMASK ) == 0 ) )
    {
      if ( ++count_reassemblies > MAX_REASSEMBLIES )
        {
          error ( "Maximum number of %d fragmented packets reached, "
                  "packets surpluses are not calculated.",
                  MAX_REASSEMBLIES );

          count_reassemblies = MAX_REASSEMBLIES;
          return -1;
        }

      // busca posição livre para adicionar dados do pacote fragmentado
      for ( size_t i = 0; i < MAX_REASSEMBLIES; i++ )
        {
          if ( pkt_ip_frag[i].ttl == 0 )  // posição livre no array
            {
              // printf("novo fragmento %x no array - %d\n", l3->id, i);
              pkt_ip_frag[i].pkt_id = l3->id;
              pkt_ip_frag[i].source_port = ntohs ( l4->source );
              pkt_ip_frag[i].dest_port = ntohs ( l4->dest );
              pkt_ip_frag[i].c_frag = 1;  // first fragment
              // pkt_ip_frag[i].ttl = LIFETIME_FRAG;
              pkt_ip_frag[i].ttl = start_timer ();
              // printf("timer setado - %f\n", pkt_ip_frag[i].ttl);

              // it's first fragment
              return 1;
            }
        }
    }

  // it's not first fragment
  return 0;
}

// verifica se é um fragmento
// return -1 para indicar que não,
// return -2, maximo de fragmentos de um pacote atingido
// qualquer outro valor maior igual a 0 indica que sim,
// sendo o valor o indice correspondente no array de fragmentos
static int
is_frag ( struct iphdr *l3 )
{
  // bit não fragmentação ligado, logo não pode ser um fragmento
  if ( ntohs ( l3->frag_off ) & IP_DF )
    return -1;

  // se o deslocamento for maior que 0, indica que é um fragmento
  if ( ntohs ( l3->frag_off ) & IP_OFFMASK )
    {
      // percorre todo array de pacotes fragmentados...
      for ( size_t i = 0; i < MAX_REASSEMBLIES; i++ )
        {
          // ... e procura o fragmento com base no campo id do cabeçalho IP
          if ( pkt_ip_frag[i].pkt_id == l3->id )
            {
              // se o total de fragmentos de um pacote for atingido
              if ( ++pkt_ip_frag[i].c_frag > MAX_FRAGMENTS )
                {
                  error ( "Maximum number of %d fragments in a "
                          "package reached",
                          MAX_FRAGMENTS );

                  pkt_ip_frag[i].c_frag = MAX_FRAGMENTS;
                  return ER_MAX_FRAGMENTS;
                }

              // se for o  ultimo fragmento
              // libera a posição do array de fragmentos
              if ( ( ntohs ( l3->frag_off ) & IP_MF ) == 0 )
                {
                  pkt_ip_frag[i].ttl = 0;
                  DEC_REASSEMBLE ( count_reassemblies );
                  // printf("removido ultimo fragmento %x no id %d\n", l3->id,
                  // i);
                }

              // fragmento localizado, retorna o indice do array de fragmentos
              return i;
            }
        }
    }

  // it's not a fragment
  return -1;
}

// Remove do array de pacotes fragmentados pacotes que
// ja tenham atingido o tempo limite de LIFETIME_FRAG definido em ttl e ainda
// tenham mais fragmentos para enviar.
// Essa ação é necessaria caso alguma aplicação numca envie um fragmento
// finalizando ser o ultimo, os fragmentos subsequentes enviados
// por esse pacote apos o tempo limite, não serão calculados
// nas estatisticas de rede do processo
static void
clear_frag ( void )
{
  for ( size_t i = 0; i < count_reassemblies; i++ )
    {
      if ( timer ( pkt_ip_frag[i].ttl ) >= LIFETIME_FRAG )
        {
          // printf("removendo pct - %x no id - %d\n", pkt_ip_frag[i].pkt_id,
          // i);
          pkt_ip_frag[i].ttl = 0;
          DEC_REASSEMBLE ( count_reassemblies );
        }
    }
}

void
insert_data_packet ( struct packet *pkt,
                     const uint8_t direction,
                     const uint32_t local_address,
                     const uint32_t remote_address,
                     const uint16_t local_port,
                     const uint16_t remote_port )
{
  pkt->direction = direction;
  pkt->local_address = local_address;
  pkt->remote_address = remote_address;
  pkt->local_port = local_port;
  pkt->remote_port = remote_port;
}
