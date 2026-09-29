module rec A : Set.S with type elt = string = Set.Make( String ) module B =
  Set.Make( Bool )
