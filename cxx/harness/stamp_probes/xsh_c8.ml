module F (X : Hashtbl.SeededHashedType) :
  Hashtbl.SeededS with type key = X.t = struct
  include Hashtbl.MakeSeeded (X)
end
