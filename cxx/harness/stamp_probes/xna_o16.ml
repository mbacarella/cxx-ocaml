module rec Mod : sig module XSet : Set.OrderedType end = struct module XSet =
  struct type t = int let compare = compare end end
