module XSet : sig type t end = Set.Make( String ) module N : sig end = struct
  module XSet = Set.Make( String ) end
