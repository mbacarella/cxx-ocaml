module F (X : sig end) = struct
  class type t = object end
  class c = object end
end;;
module M1 = struct end;;
module N = F(M1);;
class type u = F(M1).t;;
