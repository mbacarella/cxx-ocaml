module S : Map.S =
  Map.Make(struct type t = int let compare = compare end)
