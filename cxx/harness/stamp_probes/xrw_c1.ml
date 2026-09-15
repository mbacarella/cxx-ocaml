module M = struct type t = int let compare = compare end
module S : Set.S with type elt = M.t = Set.Make(M)
