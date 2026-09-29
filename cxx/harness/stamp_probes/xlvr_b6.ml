module M = Map.Make (String)
module N : sig type 'a u = 'a M.t end = struct type 'a u = 'a M.t end
