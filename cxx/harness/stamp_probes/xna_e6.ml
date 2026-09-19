module M : sig module XSet : module type of Set.Make( String ) end = struct
  module XSet = Set.Make( String ) end
