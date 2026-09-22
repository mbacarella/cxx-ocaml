module M = Hashtbl.Make(struct
  type t = int let equal = (=) let hash = Hashtbl.hash end)
