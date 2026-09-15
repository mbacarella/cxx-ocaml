module M = struct type t = int let compare = compare end
let _ = let open Map.Make (M) in empty
