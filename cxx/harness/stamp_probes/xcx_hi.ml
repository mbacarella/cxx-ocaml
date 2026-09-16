type _ c1 = A : int c1 | B : int c1 | C : char c1
type _ c2 = X : int c2 | Y : char c2 | Z : char c2
type _ repr = R1 : 'a c1 repr | R2 : 'a c2 repr
let h (type a) (r1 : a repr) (r2 : a repr) (a : a) =
  match r1, r2, a with
  | R1, _, C -> ()
  | _ -> ()
