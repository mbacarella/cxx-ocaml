(* S420 CONTROL: TWO guarded root-or rows -- ng > 1, so the licence is withheld
   and the whole match keeps the pre-S420 wiring. *)
type d = A of int | B of int | C of int * int * int | N
let g x = string_of_int x
let hh a b c = string_of_int (a + b + c)
let pm () = Sys.opaque_identity true
let f d1 d2 =
  begin match (d1, d2) with
  | (A x, A y) -> g (x + y)
  | (A _, _) | (_, A _) when pm () -> "a"
  | (B _, _) | (_, B _) when pm () -> "b"
  | (C (fl, kind, rem), N) | (N, C (fl, kind, rem)) -> hh fl kind rem
  | (_, _) -> "d"
  end
let () =
  List.iter (fun (a, b) -> print_string (f a b); print_newline ())
    [ (A 1, A 2); (A 1, N); (N, B 3); (C (1,2,3), N); (N, C (4,5,6)); (N, N) ]
