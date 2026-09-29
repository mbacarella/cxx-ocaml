module A = struct type t = int let compare = compare end module M : sig module
  XSet : sig type t end end = struct module XSet = Set.Make( A ) end
