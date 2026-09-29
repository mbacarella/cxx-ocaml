let f = function (Some x as o) -> (x, o) | None as o -> (0, o)
let g ((a, _) as p) = (a, p)
