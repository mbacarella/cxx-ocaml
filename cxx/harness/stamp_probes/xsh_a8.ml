module M = struct type t = int let compare = compare end
module A = Map.Make (M)
module G (Y : Set.OrderedType) = struct type u = Y.t end
