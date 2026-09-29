module M : sig type elt = string type t val empty : t end = struct
  include Set.Make(String)
end
