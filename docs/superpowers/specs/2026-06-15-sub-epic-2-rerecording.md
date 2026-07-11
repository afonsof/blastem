# Sub-épico 2: Integração com Save States (Re-recording)

**Épico pai:** [2026-06-13-gameplay-recording-epic.md](./2026-06-13-gameplay-recording-epic.md)  
**Data:** 2026-06-15  
**Status:** Spec — aguardando plano de implementação  
**Referência:** Snes9x `movie.cpp` (`S9xMovieFreeze`/`S9xMovieUnfreeze`) e `snapshot.cpp`

---

## Objetivo

Integrar o módulo de gravação ao mecanismo de save states do BlastEm de forma que:

- Ao **salvar** um save state durante gravação, o estado atual do movie (frame_count + input buffer) é embutido no arquivo `.state`.
- Ao **carregar** esse save state durante gravação, o input buffer é truncado naquele frame e a gravação continua a partir daí, com `rerecord_count` incrementado.
- Ao **carregar** um save state sem movie data durante gravação, a gravação é interrompida (estado incompatível).

---

## Referência Técnica — Snes9x

O Snes9x resolve exatamente este problema em `snapshot.cpp`:

| Momento | Snes9x | BlastEm (este sub-épico) |
|---------|--------|--------------------------|
| Salvar state | `S9xMovieFreeze` → bloco `MID` no arquivo | `movie_freeze(buf)` → `SECTION_MOVIE` no `.state` |
| Carregar state | `S9xMovieUnfreeze` → restaura buffer + trunca | `movie_unfreeze(buf, gen)` → restaura + trunca + flush |
| State sem movie | retorna `NOT_A_MOVIE_SNAPSHOT` | `movie_check_after_load()` → `movie_record_stop()` |

---

## Contexto do BlastEm Relevante

| Componente | Arquivo | Papel |
|---|---|---|
| Serialização | `serialize.h` / `serialize.c` | `start_section`/`end_section`, `save_int32`, `save_buffer8`, `register_section_handler`, `load_section` |
| Save de estado | `genesis.c:genesis_serialize` | Serializa estado completo; ponto de hook para `movie_freeze` |
| Load de estado | `genesis.c:genesis_deserialize` | Registra handlers por seção; ponto de hook para `movie_unfreeze` |
| Trigger de load | `genesis.c:handle_reset_requests` | Executa o load após `delayed_load_slot` — pós-deserialize |
| Movie state | `movie.c` / `movie.h` | Módulo do sub-épico 1 |

---

## Novos Símbolos

### Em `serialize.h`

Adicionar ao enum de seções, após `SECTION_COLECO_IO`:

```c
SECTION_MOVIE
```

### Em `movie.h`

```c
/* Chamado durante genesis_serialize: embute movie state em SECTION_MOVIE.
 * No-op se não estiver gravando. */
void movie_freeze(serialize_buffer *buf);

/* Handler para genesis_deserialize: trunca timeline ao frame salvo.
 * Se não gravando: ignora. Se gravando e seção ausente: movie_record_stop(). */
void movie_unfreeze(deserialize_buffer *buf, void *vgen);

/* Chamado por genesis.c após deserialize completo.
 * Para a gravação se SECTION_MOVIE não foi encontrada durante o load. */
void movie_check_after_load(void);
```

---

## Implementação

### `movie_freeze(serialize_buffer *buf)`

```c
void movie_freeze(serialize_buffer *buf)
{
    if (movie.state != BSM_STATE_RECORD) {
        return;
    }
    start_section(buf, SECTION_MOVIE);
    save_int32(buf, movie.header.frame_count);
    save_int32(buf, movie.input_buffer_used);
    save_buffer8(buf, (uint8_t *)movie.input_buffer,
                 movie.input_buffer_used * sizeof(bsm_frame_input));
    end_section(buf);
}
```

> Serializa: (1) frame_count total no disco, (2) quantos frames ainda estão no buffer em memória, (3) o conteúdo desse buffer.

### `movie_unfreeze(deserialize_buffer *buf, void *vgen)`

```c
void movie_unfreeze(deserialize_buffer *buf, void *vgen)
{
    uint32_t saved_frame_count = load_int32(buf);
    uint32_t saved_buffer_used = load_int32(buf);

    /* IMPORTANTE: sempre ler todo o conteúdo da seção antes de qualquer
     * return, caso contrário o deserialize_buffer fica em posição errada
     * e quebra o load do restante do save state. */
    if (saved_buffer_used > movie.input_buffer_cap) {
        saved_buffer_used = movie.input_buffer_cap;
    }
    load_buffer8(buf, (uint8_t *)movie.input_buffer,
                 saved_buffer_used * sizeof(bsm_frame_input));

    movie.freeze_seen = 1;   /* sinaliza para movie_check_after_load */

    if (movie.state != BSM_STATE_RECORD) {
        return;   /* safe: dados já foram lidos do buffer */
    }

    /* Flush do buffer atual antes de truncar */
    flush_inputs();

    /* Truncar o arquivo .bsm no disco */
    uint32_t trunc_pos = movie.header.input_offset +
                         saved_frame_count * sizeof(bsm_frame_input);
#ifdef _WIN32
    _chsize(fileno(movie.file), trunc_pos);
#else
    ftruncate(fileno(movie.file), trunc_pos);
#endif
    fseek(movie.file, trunc_pos, SEEK_SET);

    /* Restaurar contadores com os valores do save state */
    movie.input_buffer_used     = saved_buffer_used;
    movie.header.frame_count    = saved_frame_count;
    movie.header.rerecord_count++;

    /* Reescrever header com novos valores */
    bsm_write_header(movie.file, &movie.header);
}
```

> **Portabilidade:** `ftruncate` é POSIX. No Windows usa-se `_chsize`, igual ao que o Snes9x faz em `movie.cpp`.

### `movie_check_after_load(void)`

```c
void movie_check_after_load(void)
{
    if (movie.state != BSM_STATE_RECORD) {
        movie.freeze_seen = 0;
        return;
    }
    if (!movie.freeze_seen) {
        warning("movie_check_after_load: save state sem movie data — parando gravação\n");
        movie_record_stop();
    }
    movie.freeze_seen = 0;
}
```

### Campo interno adicional em `bsm_movie` (`movie.c`)

```c
uint8_t freeze_seen;   /* setado a 1 por movie_unfreeze, checado por movie_check_after_load */
```

---

## Hooks em `genesis.c`

### Em `genesis_serialize` — após serializar todo o estado normal

```c
#ifndef IS_LIB
    movie_freeze(buf);
#endif
```

### Em `genesis_deserialize` — junto aos outros `register_section_handler`

```c
#ifndef IS_LIB
    movie.freeze_seen = 0;   /* reset antes de cada deserialize */
    register_section_handler(buf,
        (section_handler){.fun = movie_unfreeze, .data = gen},
        SECTION_MOVIE);
#endif
```

> **Nota:** `genesis_deserialize` não tem acesso direto a `movie` (variável estática em `movie.c`). O reset do `freeze_seen` deve ser feito via função `movie_prepare_for_load()` (ver abaixo).

### Função auxiliar em `movie.h` / `movie.c`

```c
/* Deve ser chamada por genesis.c ANTES de cada genesis_deserialize.
 * Reseta a flag freeze_seen para o próximo load. */
void movie_prepare_for_load(void);
```

```c
void movie_prepare_for_load(void) { movie.freeze_seen = 0; }
```

### Após `genesis_deserialize` em `handle_reset_requests`

```c
#ifndef IS_LIB
    movie_check_after_load();
#endif
```

---

## Arquivos Modificados

| Arquivo | Tipo | Mudança |
|---------|------|---------|
| `serialize.h` | Modificar | Adicionar `SECTION_MOVIE` ao enum |
| `movie.h` | Modificar | Declarar `movie_freeze`, `movie_unfreeze`, `movie_check_after_load`, `movie_prepare_for_load` |
| `movie.c` | Modificar | Implementar as 4 novas funções; adicionar `freeze_seen` a `bsm_movie` |
| `genesis.c` | Modificar | Hook `movie_freeze` em `genesis_serialize`; registrar handler + `movie_prepare_for_load` + `movie_check_after_load` em paths de load |

---

## Testes

### Extensão de `testmovie.c`

**`test_movie_freeze_roundtrip`**  
- Inicia gravação em memória (sem arquivo real)
- Popula `movie.input_buffer` com N frames conhecidos
- Chama `movie_freeze(&buf)` → serializa
- Chama `movie_unfreeze(&deser, NULL)` → restaura
- Verifica que `frame_count` e o conteúdo do buffer batem

**`test_stop_on_missing_section`**  
- Inicia gravação
- Chama `movie_prepare_for_load()` (sem chamar `movie_unfreeze`)
- Chama `movie_check_after_load()`
- Verifica que `movie_get_state() == BSM_STATE_NONE`

---

## Critério de Aceite End-to-End

1. `./blastem -R saida.bsm jogo.bin`
2. Jogar ~300 frames → Quick Save (`ui.save_state`)
3. Jogar mais ~200 frames (total 500)
4. Carregar o save state (`ui.load_state`)
5. Jogar mais ~100 frames
6. Fechar o emulador
7. Verificar `saida.bsm`:
   - `frame_count == 400` (300 + 100, não 600)
   - `rerecord_count == 1`
   - Tamanho do arquivo = `input_offset + 400 * 4`

---

## Fora do Escopo

- Playback dos inputs gravados (Sub-épico 3)
- Múltiplos save states alternados em sequência (testado apenas um load)
- Quick save com slot personalizado (apenas Quick Save slot 10 testado no critério de aceite)
