module M = struct type t = int let compare = compare end
module G (Y : Map.OrderedType) = struct type u = Y.t end
module A = Map.Make (M)
