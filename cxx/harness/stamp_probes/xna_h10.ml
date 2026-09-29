module M : sig module rec XSet : sig type t end end = struct module XSet =
  Set.Make( String ) end
