# Flying Pet

Mod nativo C++20 para Minecraft Bedrock no Android via LeviLauncher.

A **v0.3.0** é a primeira build funcional depois da sequência de diagnósticos A–H. O crash foi isolado na variante antiga de `RenderMeshImmediately`; a versão atual usa `RenderMeshImmediately2`, que passou no aparelho de teste com `TessellatorBegin`, `TessellatorColor`, `TessellatorVertex` e envio do mesh completo retornando com segurança.

## Requisitos

- Android arm64-v8a
- LeviLauncher
- Minecraft Bedrock compatível com as signatures/offsets atuais do projeto
- Não precisa do BedrockTools instalado

## Instalação

1. Abra a aba **Actions** deste repositório.
2. Entre no workflow **Build Flying Pet** mais recente que estiver verde.
3. Baixe o artifact `FlyingPet-arm64-v8a`.
4. Extraia `FlyingPet.levipack` do artifact.
5. Importe o `.levipack` pelo gerenciador de mods do LeviLauncher.
6. Habilite o Flying Pet e abra o Minecraft pelo LeviLauncher.

## Como a v0.3.0 funciona

- Resolve as funções diretamente em `libminecraftpe.so` com `pl::memory::resolveSignature`.
- Usa `NormalTick` para ler a posição do jogador pelo `StateVectorComponent` em `Actor + 0x208`.
- O pet fica voando próximo ao jogador e faz uma órbita suave ao redor dele.
- O movimento é suavizado para o pet não teleportar a cada tick.
- O corpo é um cubo 3D em linhas, com rosto, antenas, cauda e duas asas animadas.
- A posição da câmera vem de `LevelRendererPlayer + 0x61C`.
- A renderização usa o Tessellator do Minecraft e a variante validada `RenderMeshImmediately2`.
- Há um aquecimento curto de 30 frames antes do primeiro desenho para evitar renderização durante a inicialização do mundo.

## O que foi validado nos diagnósticos

- `NormalTick`: seguro
- `RenderLevel`: seguro
- `Actor + 0x208`: seguro
- `ScreenContext + 0xB8` (Tessellator): seguro
- `LevelRenderer + 0x420`: seguro
- câmera em `+0x61C`: segura
- `ColorHolder` em `ScreenContext + 0x30`: seguro
- material em `LevelRendererPlayer + 0x1030`: seguro
- `TessellatorBegin`: seguro
- `TessellatorColor`: seguro
- `TessellatorVertex`: seguro
- `RenderMeshImmediately`: causava crash nesta build do jogo
- `RenderMeshImmediately2`: validado com sucesso

## Limitações

O pet ainda é **visual e cliente-side**. Ele não é uma entidade real do mundo: outros jogadores não veem o pet, ele não tem colisão, vida, inventário nem IA do servidor.

Como o mod usa funções e offsets internos do Minecraft, uma atualização do jogo pode exigir novas signatures/offsets.

## Próximos passos

- Menu no LeviLauncher para ativar/desativar o pet
- Escolher distância, altura e velocidade
- Mais modelos e estilos
- Textura/modelo próprio
- Nome acima do pet
- Mais animações

## Créditos técnicos

O projeto usa o Preloader Android e toma o BedrockTools como referência técnica para signatures, offsets e padrões de renderização do Minecraft Bedrock Android.
