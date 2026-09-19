module M : sig module N : sig module XSet : sig type t end end end = struct
  module N = struct module XSet = Set.Make( String ) end end
