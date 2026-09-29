module M : sig module XSet : sig type t val to_seq : t -> string Seq.t end end =
  struct module XSet = Set.Make( String ) end
