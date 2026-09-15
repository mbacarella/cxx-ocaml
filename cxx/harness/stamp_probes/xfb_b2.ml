module M = struct type t = int let compare = compare end
module S : Map.S = Map.Make (M)
let z = 0
