module F (X : sig end) = struct
  class type t = object end
  class c = object end
end;;
module M1 = struct end;;
let g () = let module N = struct class type u = F(M1).t end in ();;
let h () = let module N = struct class type u = F(M1).t end in ();;
