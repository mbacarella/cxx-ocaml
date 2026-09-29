module type Empty = sig val y : int end
module N : Empty = struct module S = Set.Make (String) let y = 0 end
