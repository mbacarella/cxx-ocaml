module M : sig module XSet : sig type t end type u = string Seq.t end = struct
  module XSet = Set.Make( String ) type u = string Seq.t end
