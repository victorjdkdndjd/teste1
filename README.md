# Flying Pet

Mod nativo C++20 para Minecraft Bedrock no Android via LeviLauncher.

A v0.2.0 é **standalone**: o mod não precisa do BedrockTools instalado. Ele usa o Preloader para localizar diretamente no `libminecraftpe.so` as funções necessárias e desenha um pequeno pet 3D cliente-side que voa perto do jogador, balança verticalmente, bate as asas e acompanha o jogador com movimento suavizado.

## Requisitos

- Android arm64-v8a
- LeviLauncher
- Minecraft Bedrock compatível com as signatures/offsets atuais do projeto

## Instalação

1. Abra a aba **Actions** deste repositório.
2. Entre no workflow **Build Flying Pet** mais recente que estiver verde.
3. Baixe o artifact `FlyingPet-arm64-v8a`.
4. Extraia `FlyingPet.levipack` do artifact.
5. Importe o `.levipack` pelo gerenciador de mods do LeviLauncher.
6. Habilite somente o Flying Pet e abra o Minecraft pelo LeviLauncher.

## Como a v0.2.0 funciona

- `pl::memory::resolveSignature` localiza funções diretamente em `libminecraftpe.so`.
- Um hook de `ClientInstanceUpdate` captura o `ClientInstance` atual.
- `ClientInstanceGetLocalPlayer` obtém o jogador local.
- A posição vem do `StateVectorComponent` do Actor.
- O alvo do pet orbita aproximadamente 1,55 bloco ao redor do jogador.
- O pet fica cerca de 1,75 bloco acima da posição do jogador.
- Um `lerp` suaviza o movimento.
- O pet é renderizado em `RenderLevel` usando o Tessellator do Minecraft.
- As asas usam uma animação senoidal simples.

## Limitações

O pet ainda é **visual e cliente-side**. Ele não é uma entidade real do mundo: outros jogadores não veem o pet, ele não tem colisão, vida, inventário nem IA do servidor.

Como o mod usa funções e offsets internos do Minecraft, uma atualização do jogo pode exigir novas signatures/offsets. Quando alguma signature não for encontrada, o Logcat do mod mostra exatamente qual nome falhou.

## Próximos passos

- Menu no LeviLauncher para ativar/desativar o pet
- Escolher distância, altura e velocidade
- Pet ficar atrás ou ao lado do jogador em vez de orbitar
- Textura/modelo próprio
- Mais tipos de pet
- Nome acima do pet
- Animações extras

## Créditos técnicos

O projeto usa o Preloader Android e toma o BedrockTools como referência técnica para signatures, offsets e padrões de renderização do Minecraft Bedrock Android.
