module F (X : Hashtbl.HashedType) : Hashtbl.S with type key = X.t =
struct
  include Hashtbl.Make (X)
end
