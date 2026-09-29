module type S = sig type v1 type v2 end
module M = struct type v1 = T of int type v2 = v1 = T of int end
module F(X : S) = struct
  type 'a wit = V1 : string -> X.v1 wit | V2 : int -> X.v2 wit
  let f : X.v1 wit -> unit = function V1 s -> print_endline s | V2 _ -> ()
end
module N = F(M)
let () = N.f (N.V2 0)
