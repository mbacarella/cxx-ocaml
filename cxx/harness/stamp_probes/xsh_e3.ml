module M = struct type t = int let compare = compare end
module A = Map.Make (M)
let _ = let open Map.Make (M) in empty
