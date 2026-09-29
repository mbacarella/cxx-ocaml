module W = struct module F ( X : Set.OrderedType ) = struct module N = Set.Make(
  String ) type u = Set.Make( X ).t end end
