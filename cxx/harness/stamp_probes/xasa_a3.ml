module M = struct type t = int let compare = compare end
module N : Map.OrderedType = M
