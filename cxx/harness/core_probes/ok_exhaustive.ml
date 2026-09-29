let f = function true -> 1 | false -> 0
let g = function [] -> 0 | [_] -> 1 | _ :: _ :: _ -> 2
let h = function (None, _) -> 0 | (Some _, true) -> 1 | (Some _, false) -> 2
