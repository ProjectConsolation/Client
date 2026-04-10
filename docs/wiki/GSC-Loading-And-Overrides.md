# GSC Loading And Overrides

Project: Consolation supports custom GSC loading and overriding stock scripts.

## How It Works

Create a `consolation` folder inside your game directory.

Inside that folder, follow the same folder structure as the stock game scripts you want to override.

## Example

If you want to override the stock Conflict gametype script, create:

`consolation/maps/mp/gametypes/dm.gsc`

This lets you replace or extend the stock `dm.gsc` behavior through the Project: Consolation script loading path.

## Practical Use

One use for this is making a custom FFA-style Conflict variant by replacing `dm.gsc` with your own logic.

Example showcase:

[![Custom Gun Game example by replacing `dm.gsc`](https://img.youtube.com/vi/0Zu-5G9qdcg/hqdefault.jpg)](https://www.youtube.com/watch?v=0Zu-5G9qdcg)
