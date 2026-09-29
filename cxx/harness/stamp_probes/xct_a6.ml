module F (X : sig end) = struct
  class type t = object end
  class c = object end
end;;
module M1 = struct end;;
let f (x : #F(M1).t) = x;;
