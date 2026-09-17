module HW = Ephemeron.K1.Make(struct type t = int let equal = (=)
let hash = Hashtbl.hash end)
