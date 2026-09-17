module Y = Map.Make (struct type t = int let compare = compare
                            module F (_ : sig end) = struct end end)
