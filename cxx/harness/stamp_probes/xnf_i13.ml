module W = struct module rec F : sig type u end = struct type u = int end and G
  : sig end = struct end module H ( X : Set.OrderedType ) = struct type u =
  Set.Make( X ).t end end
