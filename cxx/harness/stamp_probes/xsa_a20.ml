module S = struct include Int64 let hash (x:t) = Hashtbl.hash x end
module HW = Ephemeron.K1.Make(S)
module SW = Weak.Make(S)
