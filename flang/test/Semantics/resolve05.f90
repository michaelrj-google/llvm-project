! RUN: %python %S/test_errors.py %s %flang_fc1 -pedantic
program p
  integer :: p
end
module m
  !PORTABILITY: Name 'm' declared in a module should not have the same name as the module [-Wbenign-name-clash]
  integer :: m
end
submodule(m) sm
  !PORTABILITY: Name 'sm' declared in a submodule should not have the same name as the submodule [-Wbenign-name-clash]
  integer :: sm
end
block data bd
  !PORTABILITY: Name 'bd' declared in a BLOCK DATA subprogram should not have the same name as the BLOCK DATA subprogram [-Wbenign-name-clash]
  type bd
  end type
end
module m2
  type :: t
  end type
  interface
    subroutine s
      !ERROR: Module 'm2' cannot USE itself
      use m2, only: t
    end subroutine
  end interface
end module
subroutine s
  !ERROR: 's' is already declared in this scoping unit
  integer :: s
end
function f() result(res)
  integer :: res
  !ERROR: 'f' is already declared in this scoping unit
  !ERROR: The type of 'f' has already been declared
  real :: f
  res = 1
end
